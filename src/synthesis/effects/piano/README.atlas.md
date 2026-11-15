# Piano source

The Piano oscillator type runs the engine of Piano, Alessio Scala's physically
modelled concert grand (a JUCE VST3 for Apple silicon, GPL v3 or later), ported
from its `Source/dsp` folder. It uses no samples: every note is computed from a
model of the hammer, the strings, the bridge and the soundboard, voiced by
measurement against dry renders of Pianoteq's NY Steinway D and recordings of a
Yamaha C5. The plugin's `docs/model.md` describes the model and how each part
was calibrated.

## What is kept as it is

`Design.{h,cpp}`, `Soundboard.{h,cpp}`, `Model.{h,cpp}` and `Hammer.h` are the
original files: the string scale, the hammers, the fitted calibration tables,
the soundboard's modes and radiation, and the builder that turns them into
resonator tables (`buildModel`). Three lines changed: `Design.h` calls
`std::sqrt` instead of `__builtin_sqrt`, and the count of case modes the
builder appends to the board's modes is `kCaseModes` in `Model.h`, which the
engine needs. `Denormals.h` keeps the original's FPCR flush on arm64 and sets
MXCSR on x64.

The builder's development hooks (`PIANO_CAL`, `PIANO_DUMP*`, `PIANO_DEBUG*`
in the environment) are kept with it; they run on the builder thread only.

## What changed

The original `Engine` plays all 88 notes in one object and sums them. Atlas
plays each note in a synth voice, so `piano_instrument.{h,cpp}` splits the
engine in two:

- A `Lane` is one struck note, carried by a voice
  (`src/synthesis/producers/piano_source.*`): the hammer and its substeps, the
  main resonator bank, the longitudinal vibration (phantom partials), the knock
  of the action, the damper, and that note's share of the soundboard, driven
  by its own knock and thud. The board is linear, so the boards of the notes
  sum to the one board of the original; each copy prunes its modes from the
  top as they fall below -110 dB, as the main bank does, so a ringing note
  keeps only the low modes running.
- The `Instrument`, shared by every voice of the slot, keeps what couples the
  notes: the eight-region bridge motion the strings and boards feed, delayed
  one chunk (32 samples) as in the original, and the sympathetic bank of every
  note with its damper, which follows the keys held (any lane on the note) and
  the pedals. It renders once per block after the voices
  (`SynthVoiceHandler::mixSlotTails`) and joins the slot's output at its
  destination, after the voice filters and Envelope 1.

Rendered the same way, the two halves add up to the original's output:
`AtlasPianoEngineTests --reference` plays the port and the original engine
(`tests/piano/reference_engine.*`, the untouched `Engine.cpp` with its NEON
intrinsics read through `piano_simd.h`) on the same model and events, single
notes, a pedalled chord, repeats with the soft and sostenuto pedals and the
opening of a prelude as written, and the largest difference is 74 to 90 dB
below the peak (float summation order and the per-note pruning). Through the
plugin (`AtlasPianoTests --render`), the prelude's 90 seconds come out 55 dB
below the original's render of the same file with the knocks off; with them
on, the knocks' random variation draws in a different order, so the two
renders differ as two performances of the original do.

Other differences:

- Atlas's voices replace the original's 88 note states. A note struck again
  while it still sounds, in the same voice or in another (a note repeated under
  the pedal, or a held key struck again), is struck as it moves, as in the
  original: the voice handler announces the block's note-ons before any voice
  renders (`announceStrike`), the voice sounding the note stops at the strike,
  and the new voice takes its strings over there (`takeOver`). A note struck on
  a voice that still sounds another note (a stolen voice) hands the old note to
  one of eight fade slots, which the voice renders out over Atlas's voice kill
  time. The random variation of the knocks draws from one
  generator per instrument in event order, as the original does.
- The sustain pedal is the engine's own: the voice handler reports a key up
  (`key_lifted`) while Atlas's pedal keeps the voice on, so the damper and the
  key's return knock follow the key, and half pedalling works. The pedals come
  from the block's MIDI controllers (`MultiSampleMidiState`, CC 64, 66 and
  67) as events at their samples (`pedalEvent`), which every lane and the body
  apply at that sample, as the original does. A note-off without a release
  velocity (0) is taken as 64, as the original plugin takes it.
- In Piano envelope mode (the default) the note holds the voice open until it
  is 90 dB down, then what is left is dropped; the original lets everything
  decay to -110 dB. Envelope 1 mode hands the shape to Env 1, and a voice
  freed by it drops its note.
- The model is built on one background thread per process
  (`piano_model_service.*`), as the original plugin's worker builds it: a
  change to Tuning, Octave stretch, Unison width or Sustain length (or the
  sample rate) rebuilds once the controls have stopped moving, and the voices
  fade the block out when the new model takes over. Instruments that want the
  same settings share one model. Until the first model is ready a note is
  silent; offline renders wait for it.
- The original's Volume is the slot's level. A slot's audio reaches the
  effects at half its raw level, so the raw output is the engine's times two
  (`PianoSource::kOutputScale`): the level's default (70.7%, 0.5 after the
  level curve) plays the piano at the plugin's default Volume of -6 dB, and
  100% at 0 dB. Its reverb is not ported; Atlas's effects follow the slot.
- Transpose is an Atlas addition. Pitch bend and glide do not move a struck
  note, and a legato note change in mono mode (no note-on) leaves the first
  note sounding: the model holds each note's partials in tables.
- The voice's lane buffers are sized for the largest note (2,560 resonators,
  the board's 1,280 modes), so a note-on allocates nothing.
- The board bus to the sympathetic strings is computed per lane, so a passage
  with every key ringing costs about one and a half times the original.

## Build

These files are listed in `ATLAS_PIANO_SOURCES` in `CMakeLists.txt` and built
outside the fast-math unity build with IEEE arithmetic, as the original is.

## Testing

`AtlasPianoEngineTests` (`tests/piano/engine_tests.cpp`, no JUCE) plays the
port against the original engine (`--reference`), checks the original's own
behaviour tests on the port and the fade of a stolen note (`--behaviour`), and
times every key ringing with the pedal down (`--cpu`). `AtlasPianoTests`
(`tests/piano_tests.cpp`) covers the type through the plugin: pitch, velocity,
the damper, the range, the pedals and the sympathetic strings, the envelope
modes and the voice hold, voice stealing, host parameters and state, and the
editor's rows.
