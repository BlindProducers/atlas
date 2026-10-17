# Atlas

Atlas is an open-source, fully accessible AU/VST3 synthesizer for macOS and Windows. Built on the foundation of Matt Tytel's [Vital](https://github.com/mtytel/vital), Atlas preserves the original sound engine while significantly expanding it with more oscillator engines, filter types, effects, modular routing, and modulation sources.

Accessibility is a core priority: every parameter can be navigated and adjusted via the keyboard, with full screen reader support through VoiceOver on macOS and NVDA, JAWS, or Narrator on Windows.

---

## Oscillators

Atlas includes eight oscillator slots, each capable of running one of ten synthesis engines:

- **Wavetable**: Vital’s wavetable engine featuring unison, distortion modes, frequency morphing, and secondary morph targets. Includes an integrated wavetable editor, audio import (splice, vocode, or pitched pitch-tracking), and wavetable/.wav export.
- **Analog**: Classic virtual analog waveforms with shape controls, a dedicated sub-oscillator, and vintage drift/aging parameters.
- **Sample**: Standard audio playback with custom start, loop, and direction settings—useful for one-shots, transient attacks, and ambient textures.
- **Granular**: A granular engine with control over grain size, spray, density, and playback randomization.
- **Spectral**: Resynthesizes audio using spectral data, offering time-stretch, spectral blur, pitch-shift, freeze, and transient shaping.
- **Noise**: Multiple noise profiles with dedicated low- and high-pass filtering.
- **Multi-sample**: Plays SFZ, Decent Sampler, and SF2 soundfont files. Includes quick-actions to open SFZ/Decent Sampler definitions directly in your default text editor, hot-reloading on save, support for custom Decent Sampler UI parameters, and SF2 preset browsing.
- **[Braids](https://github.com/TOILmodular/Braids)**: An adaptation of the Mutable Instruments macro oscillator containing over 40 distinct synthesis models, ranging from vintage analog models to physical modeling and speech synthesis.
- **[OpenWurli](https://github.com/hal0zer0/openwurli)**: A physical model of the Wurlitzer 200A electric piano (pairs with the OpenWurli amp and tremolo in the effects section).
- **[Open B3](https://github.com/michele-perrone/OpenB3)**: Tonewheel organ emulation with functional drawbars, percussion, and an authentic rotary speaker simulation.

All oscillators share pitch, pan, and level controls, and can cross-modulate each other via frequency modulation (FM) and ring modulation (RM).

---

## Filters

Atlas features two voice filters configured in series or parallel, with independent output routing per filter.

In addition to Vital's models (Analog, Dirty, Ladder, Digital, Diode, Formant, Comb, and Phaser), Atlas adds:

- **Wavetable**: Uses any loaded wavetable frame as a custom filter frequency response curve.
- **Distortion**: Runs Atlas’s distortion algorithms per-voice before summing, preventing intermodulation distortion.
- **Reverb & Scream**
- **Ring Modulation & Sample & Hold**

---

## Effects & Routing

### Effect Rack & Buses

Atlas contains over 35 effects covering everything from EQ and compression to advanced probabilistic delays and convolution reverb. Effects are loaded across four independent chains: a **Main chain** and **three auxiliary buses (Bus 1 through 3)**. Each chain can hold up to 20 re-orderable effect slots.

- **Routing matrix**: Oscillators and filters can route to the Main chain, Bus 1–3, or bypass effects entirely directly to the master output.
- **Bus nesting**: Auxiliary buses can route into other buses, into the Main chain, or straight to the master output.
- **Latency compensation (PDC)**: Lookahead effects and heavy spectral processing stay phase-aligned with parallel dry signals.
- **Preset management**: Individual slots or full chains can be copied, pasted, saved, or loaded with or without their modulation assignments.

### Effect Processors

- **Analogue**: Chorus, Flanger, Echo, Rotary, Spring Reverb, Wah, and Tape.
- **Dynamics**: Compressor (single and multi-band), Limiter, and Suppressor (dynamic resonance suppression).
- **Space**: Algorithmic Reverb, Convolution Reverb (with predelay, shape fades, and tone controls), Delay, Floaty Delay, and Glitch Delay (based on [Astralay](https://github.com/ironcross32/astralay)).
- **Modulation**: Chorus, Flanger, Phaser, Tremolo/Panner, Dimension Expander, Phase Shifter, Frequency Shifter, and Trance Gate.
- **Distortion & Character**: Distortion, Waveshaper, Erosion, and Resonator.
- **Filter & EQ**: Parametric EQ, Mid/Side EQ, FX Filter, and Defuse.
- **Spectral**: Spectral Gate, Spectral Compressor, Spectral Balance, and Spectral Lock (scale snapping via filterbank or retuning).
- **Utility**: Gain, Stereo Width, and High/Low Cut filters.

---

## Modulation

- **12 LFOs**: Fully custom curve editor accessible via keyboard. Operates in Free (Hz), Tempo Sync (straight, dotted, triplet), or Key Track modes. Includes six trigger modes (Trigger, Sync, Envelope, Sustain Envelope, Loop Point, and Loop Hold) plus controls for stereo phase, smoothing, fade, delay, swing, and deform.
- **10 Envelopes**: DAHDSR envelopes with adjustable curve tension and tempo sync. Envelope 1 includes an optional analog mode with RC circuit curves and simulated component drift.
- **4 Random Generators**: Perlin Noise, Sample & Hold, Sine Interpolate, and Lorenz Attractor—available in mono or stereo, free or tempo-synced.
- **16 Macros & MIDI Sources**: Map velocity, channel aftertouch, mod wheel, pitch bend, key tracking, and 16 user macros.
- **Audio-Rate Modulation**: Oscillators can be chosen as modulation sources to drive parameters like pitch, phase, shape, and filter cutoffs at audio rate (per-sample) directly through the matrix.
- **Modulation Matrix**: Modulate nearly any parameter with bipolar/unipolar modes, remap curves, and stereo offsets. Parameter values can be typed directly in their native units (semitones, Hz, dB, etc.).
- **Dual Arpeggiators**: Two independent arpeggiators with customizable pattern styles, step counts, octave ranges, gate, repeats, and swing, assignable to specific oscillators.

---

## Building from Source

### Prerequisites

- CMake 3.22+
- A C++17-capable compiler:
  - **macOS**: Xcode Command Line Tools
  - **Windows**: Visual Studio 2022 (with the *Desktop development with C++* workload installed to provide the Windows SDK and MIDL compiler for the NVDA speech bridge)

### Clone & Compile

Atlas pins dependencies (including JUCE 8) as Git submodules inside `third_party/`:

```sh
# Clone with required submodules
git clone --recurse-submodules https://github.com/BlindProducers/atlas.git
cd atlas

# Configure and compile
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```

Compiled binaries are located in `build/Atlas_artefacts/Release`:
- **macOS**: Copy `Atlas.component` to `~/Library/Audio/Plug-Ins/Components` (or run `installer/macos/build_pkg.sh` to generate an installer package).
- **Windows**: Copy the entire `Atlas.vst3` folder (including its internal asset bundle for the Tape effect) to `C:\Program Files\Common Files\VST3` (or compile `installer/windows/Atlas.iss` using Inno Setup).

Atlas stores user presets, wavetables, samples, and effect chains in
`Documents/Blind Producers Plugins/Atlas`.

### Pre-built Betas

Automated builds run on every push to `dev`. You can download the latest installers (`.pkg` for macOS, `.exe` for Windows) and standalone plugin bundles from the GitHub Releases page.

*(Note: Atlas is actively in beta. Using a safety limiter downstream in your DAW monitor chain is recommended.)*

### Running Tests

Unit and timing tests are disabled by default. To enable and run them:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DATLAS_BUILD_TIMING_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure --parallel 1
```

To run reference tests that compare Atlas models to their original upstream codebases, pull the optional submodules first:

```sh
git submodule update --init --checkout third_party/aquanode-plugins third_party/WDL third_party/nih-plug
```

*(Note: `nih-plug` requires `rustc` and Python 3 installed).*

---

## License

Atlas is released under the GNU General Public License v3.0 (GPL-3.0), matching the license of Vital. See `LICENSE` for details.

Third-party libraries, audio algorithms, and adaptations used in Atlas are credited in `NOTICE` with their respective licenses. *Vital* is a trademark of Matt Tytel; this project is an independent fork and is not affiliated with or endorsed by the original author.
