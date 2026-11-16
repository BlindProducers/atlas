# Flute source

The Flute oscillator type runs the engine of Modeled Flute, Zachary Bennoui's
physically modelled concert flute (https://github.com/ZachB100/modeled-flute,
AGPL v3, commit 6749872). It uses no samples: a jet-drive and waveguide-bore
model with turbulence noise makes the sound, and a virtual breath (the
`BreathPerformer`) turns the keys, velocity and controllers into breath
pressure, pitch and vibrato. The original's `docs/milestones` reports describe
the model and the measurements behind each constant.

## What is kept as it is

The `mf` folder and `FluteEngine.cpp`, `FluteVoice.cpp` and
`BreathPerformer.cpp` are the original's `dsp` library, unchanged: the engine,
the note stack, the breath performer, the tone macros and the DSP blocks
(delay line, one-pole, DC blocker, jet nonlinearity, noise, smoother, the
denormal guard).

## What changed

The original plugin is monophonic: one `FluteVoice` played by the keys. Atlas
keeps that. `flute_instrument.{h,cpp}` is its `PluginProcessor` without the
reverb, output gain and meter: the same 20 ms smoothing of Air, Brightness,
Vibrato depth and Tuning, the block cut at every event's sample, the same
MIDI handling (dynamics controller, expression, mod wheel, sustain pedal with
the sounding note released last, all notes off). One `Instrument` per slot is
shared by every voice.

- The keys reach it directly. `SynthVoiceHandler::noteOn` and `noteOff`
  forward each key event to every Flute slot the key plays (an arpeggiator's
  channel names its slot; the keyboard plays every slot without an active
  arpeggiator), before the voices handle it, so the flute's tonguing and
  slurs are the original's whatever Atlas's voice count, Legato or voice
  override. The controllers come from the block's MIDI (`MultiSampleMidiState`,
  at their samples), the pitch wheel from the voice handler at the block's
  start with Atlas's bend range (the original has its own Bend parameter).
  All Controllers Off leaves no event in the block, so a controller the state
  no longer holds returns to its default at the block's start.
- It renders once per block ahead of the voices (`FluteRenderer`, a processor
  in the voice handler's mono router, which also reads the slot's controls,
  modulated once per block). The lane of the voice the voice handler started
  last carries the audio (`FluteSource`: the instrument's owner, by activation
  number); every other lane is silent. When a new voice takes over inside a
  block, the old lane carries the audio up to that sample and the new one
  from there, so the flute plays on through voice stealing and retriggering.
  Envelope 1 and the voice filters therefore shape it like any other type.
- Flute envelope mode (the default) holds the carrying voice open while
  breath or tone remains; Envelope 1 mode hands the shape to Env 1, and a
  voice freed by it stops carrying the audio while the flute's own release
  goes on unheard, as it would in the original after its Output were cut.
- The voice's delay lines are allocated once, when the slot's module is
  built, for 192 kHz at eight times oversampling; preparing for the rate in
  use reuses them, so the audio thread never allocates. The model runs at the
  voices' rate (the host's times Atlas's oversampling); at 1x it is sample
  for sample the original.
- A slot switched off lets its keys go and renders nothing.
- Transpose is an Atlas addition. A key is remembered by the number it was
  played at, so a Transpose change while it is down releases the right key.
- Atlas maps the pitch wheel's 14 bits onto -1 to 1 whole, so a centred wheel
  would sit a hair above 0; the instrument snaps it to 0, the original's
  centre. A full upward bend is then 1/8192 of the range short of the
  original's.
- The original's Output is the slot's level: the level's default (70.7%, 0.5
  after the level curve) plays the flute at 0 dB (`FluteSource::kOutputScale`).
  Its reverb and room size are not ported; Atlas's effects follow the slot.

## Build

These files are listed in `ATLAS_FLUTE_SOURCES` in `CMakeLists.txt` and built
outside the fast-math unity build with IEEE arithmetic, as the original is.

## Testing

`AtlasFluteTests --reference` (`tests/flute_tests.cpp`) plays a phrase of
tongued notes, slurs up and down, a register-changing slur, a note re-tongued
during a release, a key held by the sustain pedal and controller moves
through the plugin and through the original plugin's processing of a
`FluteVoice` (ported into the test without its reverb and output gain), with
eight voices, one voice with and without Legato, the Steal override and two
voices, and expects the same samples each time. The other flags cover
articulation (slurs, tonguing, the slur back to a held key), the oscillator
(pitch, velocity, Transpose, Tuning, Air), the envelope modes and the voice
hold, host parameters and state, and the editor's rows; `--render <midi> <wav>`
plays a MIDI file through it for listening.
