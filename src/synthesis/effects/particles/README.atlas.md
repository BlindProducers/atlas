# Particles source

The Particles effect runs the granular processor of Mutable Instruments Clouds
by Emilie Gillet. It is published under the MIT licence at
https://github.com/pichenettes/eurorack (`clouds/`), with its support library
at https://github.com/pichenettes/stmlib. The licence text is in each file's
header and in `../stmlib/LICENSE`.

## Naming

The eurorack README asks that derivative works not use the name "Mutable
Instruments" and recommends not keeping the module's name. Everything a user
sees or hears, and all of Atlas's own code, calls the effect Particles:
parameter ids (`particles_*`), strings, help, tests and file names. Only the
copied files keep their original folder and namespace (`clouds/`, `clouds::`),
so they stay comparable with the original. `NOTICE` credits the code, as the
MIT licence requires.

## Files

Copied from eurorack c25ff2b2226cf9f3f9be85a5ff01fbe05f5f72ed (the commit the
Braids port uses; master's `clouds/dsp` is the same), with stmlib at the commit
that tree pins, e3bd7c9cc00e4364166f9905c0509b6ffd0535ec:

- `clouds/dsp/`: the granular processor and everything it includes: the
  granular, stretch (WSOLA) and looping players, the phase vocoder (`pvoc/`),
  the diffuser, reverb and pitch shifter (`fx/`), the correlator, the µ-law
  codec and the sample-rate converter.
- `clouds/resources.{h,cc}`: the lookup tables.
- `clouds/drivers/debug_pin.h`: included by `granular_processor.cc`; it
  compiles to nothing under `TEST`.
- `../stmlib/stmlib/`: shared with the Braids port. Particles added
  `dsp/{dsp.h,filter.h,cosine_oscillator.h,parameter_interpolator.h,atan.h,atan.cc,rsqrt.h,units.h,units.cc}`,
  `utils/buffer_allocator.h` and `fft/shy_fft.h`, unchanged.

`source_sha256.json` holds the originals' checksums. Every copied file matches
except `clouds/dsp/window.h`.

Left out: the drivers, the UI and its settings, the CV scaler (Atlas's
parameters stand in for it), the four save/load memories, and the Blend knob's
soft takeover (its four parameters are separate controls here).

## Change

- `clouds/dsp/window.h` (marked `Atlas:`): `Window::Start` sets `done_ = false`
  again. It used to do so twice. In March 2023, commit 0e3756f removed one
  assignment and commit fbb53ba the other, and their merge (d1d8839) kept both
  removals. After that a stretch-mode window never restarts once it has
  finished, so the pitch shifter and time stretcher plays nothing. The engine
  test found this: the original falls silent in that mode, built as its own
  desktop test builds it.

## The engine (`particles_engine.{h,cpp}`)

`particles_engine.cpp` is the only Atlas file that includes the processor's
headers. It is built outside the unity files, like the copied sources, with
`TEST` (stmlib's host build: portable C in place of ARM assembly), `-fwrapv`
and strict floating point. `DONT_SET_USING_JUCE_NAMESPACE` keeps `juce::Random`
away from `stmlib::Random`.

- **Memory.** The processor lives in zeroed storage with the module's two
  memories (118,784 and 65,408 bytes), from `allocateZeroed`. On the module
  these sit in zeroed RAM, and `Init` leaves several members (the freeze filter,
  the playback mode, the parameters, the feedback buffer) as it finds them.
  The effect shell builds the engine only while a slot selects Particles.
- **Rate.** The processor runs at its own 32 kHz in 32-sample blocks. Maim's
  windowed-sinc resampler (`maim/resampler.h`) takes Atlas's processing rate
  down to 32 kHz and back, and the processor calls `Process` then `Prepare`
  each block, as the firmware's desktop test does. The reported latency is
  fixed for a given rate: the down-sampler's half width, a block and the
  up-sampler's half width. At 32 kHz it is one block.
- **Levels.** Input goes in through Input Gain and clips at 16 bits, as the
  module's converter does. The module halves its output into the converter,
  and its equal-power crossfade leaves either end at 0.707; its analogue stage
  makes up for both. The engine scales the output by 2√2, so at Mix 0 the dry
  signal leaves at the level it came in.
- **Controls.** The processor reads its tables with `Interpolate(table, x,
  size)`, which at x = 1 also reads the entry past the end, weighted by zero.
  The engine stops the 0-to-1 controls at 0.99998 so it never does. The grain
  envelope makes the same read inside the processor, which the engine cannot
  reach; there the neighbouring table's entry is finite and weighted by zero.
- **Trigger.** The module's TRIGGER input sets both `trigger` and `gate` for
  one block. `ParticlesModule` fires it on every note-on (it counts the
  `NoteMonitor`'s note-ons) and whenever Trigger rises through one half.
- **Platforms.** The correlator shifts a 32-bit word by 32 when a candidate
  starts on a word boundary. arm64 and x64 both treat that as a shift by 0,
  unlike the module's Cortex-M4. So Mac and Windows agree with each other,
  though not exactly with the module.

## Tests

`AtlasParticlesTests` (`tests/particles_tests.cpp`):

- `--engine`: at 32 kHz the engine matches the processor driven directly
  (`renderReference`), sample for sample, in every mode and quality.
- `--latency`: a dry impulse arrives at the reported latency from 32 kHz to
  384 kHz, and a dry tone leaves within 0.3 dB of its input level.
- `--modes`: every mode and quality sounds, a frozen buffer keeps playing, and
  a quality change stays finite.
- `--trigger`: through the plugin, with Density at its centre, a frozen buffer
  plays only when a note-on or Trigger fires a grain.
- `--parameters`: every control in every slot and bus is present with its
  version, every slider and Freeze is a modulation destination, and state
  survives a reload.
- `--editor`: the rows' order, labels, readouts and menus, and that every
  slider is offered for modulation.
- `--cpu` (not a ctest) times each mode.
