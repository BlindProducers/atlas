# Atlas

Atlas is a software synthesizer built to be played and programmed entirely with a
keyboard and a screen reader. It is based on [Vital](https://github.com/mtytel/vital)
by Matt Tytel and keeps Vital's sound engine, then extends it with more oscillators,
filters, effects, effect routing and modulation. Every control is reachable from the
keyboard and announced by VoiceOver on macOS and by screen readers on Windows.

Atlas builds as a VST3 plugin on macOS and Windows, and as an Audio Unit on macOS.

## The editor

The editor shows the whole synth as labelled groups of controls. Tab and the arrow
keys move between them, Enter types a value, and Backspace resets a control.
Shift M opens a control's context menu to add modulation, MIDI learn or reset it.
Comma and period jump between sections. The Editor menu holds tuning, accessibility
settings, the keyboard help and the list of MIDI learn mappings. The Preset menu saves,
initialises, imports and exports patches and banks, and the preset browser searches
and filters the library by bank, category, tag and favourites.

## Oscillators

Atlas has eight oscillators. Each one chooses a **Type**:

- **Wavetable**: Vital's wavetable oscillator, with frame position, unison, the
  distortion and frequency morph modes, and a secondary mode with its own amount.
  Each oscillator has a full wavetable editor: layers of wave, line and audio file
  sources with modifiers and keyframes, audio import as a splice, vocoded or pitched
  table, and export as a wavetable or WAV file.
- **Analog**: a virtual analog oscillator with classic waveforms, shape and a sub
  oscillator.
- **Sample**: plays an audio file with start, loop and playback controls.
- **Granular**: plays a sample as clouds of grains, with density, size and spread.
- **Spectral**: resynthesises a sample from its spectrum, with shift, stretch, blur,
  freeze and transient controls.
- **Noise**: several noise colours with width and low and high cuts.

Every oscillator reads in the same order: on/off, solo, type, level, pan and output
destination, then the type's controls, pitch and unison (with a scale lock), and the
warp modes. Oscillators can modulate each other with FM and ring modulation.

## Filters

Two filters with Vital's models (Analog, Dirty, Ladder, Digital, Diode, Formant,
Comb, Phaser) plus Atlas's Reverb, Scream, Ring Mod, Sample & Hold and Wavetable
models. The Wavetable model filters with any wavetable's frames as a response curve.
The filters run in parallel or in series and each sends its output to its own
destination.

## Effects

Effects are added to a chain, one per slot, in any order. They are grouped as:

- **Analogue**: Analogue Chorus, Flanger, Echo, Rotary, Spring and Wah, modelled on
  classic bucket-brigade, tape, rotating-speaker, spring and wah circuits, and Tape.
- **Dynamics**: Compressor, Limiter and Suppressor, which tames resonances.
- **Space**: Reverb, Convolution Reverb (with predelay, fades and tone), Delay and
  Floaty Delay.
- **Modulation**: Chorus, Flanger, Phaser, Tremolo Pan, Dimension Expander, Phase
  Shift, Frequency Shifter and Trance Gate.
- **Distortion and character**: Distortion, Waveshaper, Erosion and Resonator.
- **Filter and EQ**: Filter FX, EQ, Mid/Side EQ and Defuse.
- **Spectral**: Spectral Gate, Spectral Compressor, Spectral Lock (which locks the
  sound to a scale, by filterbank or by retuning) and Spectral Balance.
- **Utility**: gain, stereo width and low and high cuts.

Each effect slot can be moved, copied with or without its modulation, pasted into
another chain, replaced or removed, and each effect saves and loads its own presets.

## Effect routing

Atlas has a main effects chain and three buses, each a chain of up to twenty effect
slots of its own.

- Every oscillator and filter chooses where its sound goes: through the filters, to
  the main effects chain, to Bus 1, 2 or 3, or straight to the output.
- Each bus then sends its result to the main chain, to another bus, or straight out.
- Parallel paths are latency compensated, so effects with lookahead stay aligned
  with the dry signal.
- A whole bus, its effect order and settings, saves and loads as a bus preset, and a
  bus can be switched off or cleared in one step.

## Modulation

- **12 LFOs**, each drawn as a shape of points and curves with a keyboard editor.
  Rates are free in Hz, synced to the tempo (straight, dotted or triplet) or follow
  the played note. Each LFO runs in one of six modes: Trigger restarts with every
  note; Sync follows the song position, and when the transport is stopped starts with
  the first note you play; Envelope plays once; Sustain Envelope holds at a point;
  Loop Point and Loop Hold loop part of the shape. Fade, delay, smoothing, stereo
  phase, deform and swing shape it further.
- **10 envelopes** with delay, attack, hold, decay, sustain and release, curve
  controls and tempo-synced times.
- **4 random LFOs**: Perlin, Sample & Hold, Sine Interpolate and Lorenz Attractor,
  free or tempo synced, mono or stereo.
- **16 macros**, plus velocity, aftertouch, mod wheel, pitch bend, note and other
  MIDI sources.
- Any source modulates any parameter. Each route has an amount (typed in the
  destination's own units), bipolar or unipolar polarity, stereo, a curve and a
  remap shape.
- **Two arpeggiators** with styles, steps, gate, repeats, velocity and hold, at
  tempo-synced or free rates, each playing the oscillators you choose.

## Building

Every dependency, including JUCE 8, is in `third_party`; nothing else needs to be
downloaded. You need CMake 3.22 or newer and a C++17 compiler: Xcode command line
tools on macOS, or Visual Studio 2022 with Desktop development with C++ on Windows.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The plugins are written to `build/Atlas_artefacts/Release`. Copy the whole
`Atlas.vst3` folder, which includes the Tape effect's resources, to your VST3 folder.
On macOS the Audio Unit goes to `~/Library/Audio/Plug-Ins/Components`.

Atlas keeps its presets, wavetables, samples and effect presets in
`Documents/Blind Producers Plugins/Atlas`.

The test suite is off by default. To build and run it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DATLAS_BUILD_TIMING_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --parallel 1
```

## Licence

Atlas is free software under the GNU General Public License version 3, like Vital;
see `LICENSE`. `NOTICE` lists the third-party code Atlas includes or adapts and its
licences. Vital is a trademark of its author and is not used for Atlas.
