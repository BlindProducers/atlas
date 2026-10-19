# Atlas

Atlas is a software synthesizer built to be played and programmed entirely with a
keyboard and a screen reader. It is based on [Vital](https://github.com/mtytel/vital)
by Matt Tytel and keeps Vital's sound engine, then extends it with more oscillators,
filters, effects, effect routing and modulation. Every control is reachable from the
keyboard and announced by VoiceOver on macOS and by screen readers on Windows.

Atlas builds as a VST3 plugin on macOS and Windows, and as an Audio Unit on macOS.

## The editor

The editor shows the whole synth as labelled groups of controls. Tab and the arrow
keys move between them, Enter types a value, and Backspace resets a control. Levels
and modulation amounts read as percentages. Choices that change which controls are
shown, such as an oscillator's Type or a filter's Model, are menus: the arrow keys
step through the choices, and Enter or Space opens the list. Focus stays on the menu
when the controls around it change.
Shift M opens a control's context menu to add modulation, MIDI learn or reset it.
Comma and period jump between sections. The Editor menu holds tuning, accessibility
settings, the keyboard help and the list of MIDI learn mappings. The Preset menu saves,
initialises, imports and exports patches and banks, and the preset browser searches
and filters the library by bank, category, tag and favourites. The Community presets and
Community banks rows of its Collection list show the patches and banks shared on
[atlas.blindproducers.org](https://atlas.blindproducers.org), filtered the same way; Return
downloads a patch into the Community library and loads it, or downloads a bank and unpacks
it into the Atlas folder, and P plays the recording its uploader attached, when there is one.
The Preset menu's Share patch and Share bank items upload the current patch or a folder of
the user library to the same site, with the API token from your account there.
Settings shows the version, links to this repository, and logs in to the site or turns
the community library off.

Atlas's own announcements, such as the cursor position in the LFO, Trance gate and
wavetable editors, are spoken by the screen reader. On Windows they go straight to
NVDA's speech and braille. With another screen reader they are sent as UI Automation
notifications. Atlas never speaks with the Windows SAPI voice. On macOS they go to
VoiceOver. The Accessibility group of Settings, in the Editor menu, chooses which kinds of
announcement you hear.

## Oscillators

Atlas has eight oscillators. Each one chooses a **Type**:

- **Wavetable**: Vital's wavetable oscillator, with frame position, unison, the
  distortion and frequency morph modes, and a secondary mode with its own amount.
  Each oscillator has a full wavetable editor: layers of wave, line and audio file
  sources with modifiers and keyframes, audio import as a splice, vocoded or pitched
  table, and export as a wavetable or WAV file.
- **Analog**: a virtual analog oscillator with classic waveforms, shape, a sub
  oscillator, and aging controls for pitch drift, per-voice spread, worn waveforms,
  power supply sag, hiss and crackle, and mains hum.
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
Comb, Phaser) plus Atlas's Reverb, Scream, Ring Mod, Sample & Hold, Wavetable and
Distortion models. The Wavetable model filters with any wavetable's frames as a
response curve. The Distortion model is the Distortion effect, with all six types
and its filter, working on each note on its own, so chords gain no extra tones
between their notes.
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
  controls and tempo-synced times. Envelope 1 also has an analog mode with RC curves
  and its own Age control for worn timing.
- **4 random LFOs**: Perlin, Sample & Hold, Sine Interpolate and Lorenz Attractor,
  free or tempo synced, mono or stereo.
- **16 macros**, plus velocity, aftertouch, mod wheel, pitch bend, note and other
  MIDI sources.
- **Oscillators as sources**: each oscillator's audio can modulate the audio-rate
  controls of the other oscillators (pitch, level, phase, shape and so on) and of
  the voice filters, sample by sample, like FM routed through the modulation
  matrix. An oscillator cannot modulate its own controls.
- Any source modulates any parameter. Each route has an amount from -100% to 100%
  (typed in the destination's own units when assigned), bipolar or unipolar
  polarity, stereo, a curve and a remap shape. A slot in use has a Clear modulation
  slot button.
- **Two arpeggiators** with styles, steps, octave, gate, repeats, velocity and hold, at
  tempo-synced or free rates, each playing the oscillators you choose. An LFO,
  envelope, wheel or other source can modulate the rate and the time mode.

## Building

JUCE 8 and the other libraries Atlas builds against are git submodules in
`third_party`, each pinned to the exact version Atlas is tested with. Clone with
them:

```sh
git clone --recurse-submodules https://github.com/BlindProducers/atlas.git
```

In a clone made without them, run `git submodule update --init`. You need CMake
3.22 or newer and a C++17 compiler: Xcode command line tools on macOS, or Visual
Studio 2022 with Desktop development with C++ on Windows. That workload includes
the Windows SDK, whose MIDL compiler builds Atlas's NVDA speech client from
`third_party/nvda_controller`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The plugins are written to `build/Atlas_artefacts/Release`. Copy the whole
`Atlas.vst3` folder, which includes the Tape effect's resources, to your VST3 folder
(`C:\Program Files\Common Files\VST3` on Windows).
On macOS the Audio Unit goes to `~/Library/Audio/Plug-Ins/Components`.

On macOS, `installer/macos/build_pkg.sh` turns that build into an installer,
`dist/Atlas-<version>-macOS.pkg`, which installs the VST3 and the Audio Unit for
all users in `/Library/Audio/Plug-Ins`. On Windows, `installer/windows/Atlas.iss`
(Inno Setup) builds `Atlas-<version>-Windows.exe`, which installs the `Atlas.vst3`
folder into `C:\Program Files\Common Files\VST3`, or a folder you choose, and
can be removed from Apps & features.

Every push to `dev` is built for both platforms on GitHub Actions
(`.github/workflows/release.yml`), and the installers and bare plug-ins are
published together on the Releases page as the current beta pre-release
(`v2.0.0-beta.1` and so on; the beta number is set at the top of the workflow).
Download the `.pkg` or `.exe` for your platform, or a zip to copy the plug-in by
hand, then rescan plug-ins in your host. Atlas is in beta: a limiter after it is
recommended, and contributions are welcome.

Atlas keeps its presets, wavetables, samples and effect presets in
`Documents/Blind Producers Plugins/Atlas`.

The test suite is off by default. To build and run it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DATLAS_BUILD_TIMING_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --parallel 1
```

Some tests compare Atlas against the original code it adapts. Those sources are
opt-in submodules that a normal clone skips; fetch them and configure again to
add their tests:

```sh
git submodule update --init --checkout third_party/aquanode-plugins third_party/WDL third_party/nih-plug
```

`aquanode-plugins` (about 130 MB) holds the original Spectral Lock, `WDL` runs the
original JSFX effects with EEL2, and `nih-plug` holds Diopser for the Defuse
reference, which also needs `rustc` and Python 3. `third_party/README.atlas.md`
describes each dependency.

## Licence

Atlas is free software under the GNU General Public License version 3, like Vital;
see `LICENSE`. `NOTICE` lists the third-party code Atlas includes or adapts and its
licences. Vital is a trademark of its author and is not used for Atlas.
