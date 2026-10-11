# Multi-sample oscillator (SFZ and SF2)

The Multi-sample oscillator type plays SFZ and SoundFont 2 instruments. The
parsers, loader and player are written for Atlas from the public format
descriptions: the SoundFont 2.04 specification and the opcode reference at
sfzformat.com. No code from another SFZ or SF2 player is used, so `NOTICE` is
unchanged. Behaviour matches sfizz and FluidSynth where noted below.

## Files

- `multisample_instrument.*`: the immutable instrument model (regions, lookups, CC defaults).
- `sfz_parser.*`, `sf2_parser.*`: text and RIFF parsing.
- `multisample_loader.*`: decoding (WAV, AIFF, FLAC, Ogg) to 16-bit, CRC32 and the content hash.
- `multisample_container.*`: self-contained presets and the instrument cache.
- `multisample_manager.*`: background loading, per-slot state, presets and host state.
- Playback is in `src/synthesis/producers/multisample_{player,slot,source}.*` and `src/synthesis/modules/multisample_module.*`.

## SFZ support

- **Headers:** `<control>`, `<global>`, `<master>`, `<group>` and `<region>`, inherited in that order. A new header resets the levels below it. `<curve>`, `<effect>`, `<midi>` and `<sample>` blocks are skipped and reported.
- **Preprocessor:**
  - `#define $NAME value`, kept across included files. The longest name wins when names share a prefix.
  - `#include "file"`, resolved from the main file's folder as sfizz does, with cycle and depth guards.
  - `//` and `/* */` comments.
  - Backslash paths.
  - `default_path`, `note_offset` and `octave_offset`.
  - Sample names containing spaces.
  - Note names (`c4` = 60).
  - A path that doesn't match exactly is retried ignoring case.
- **Mapping:** `lokey`, `hikey`, `key`, `lovel`, `hivel`, `lorand`, `hirand`, `seq_length`, `seq_position`, `locc`/`hicc` and `lohdcc`/`hihdcc`, `sw_last`, `sw_lokey`, `sw_hikey` and `sw_default`. Round-robin counters advance whatever the CC state, so mic layers stay in step.
- **Triggers:**
  - `trigger=attack|release|release_key|first|legato`. `first` and `legato` play like `attack`.
  - `on_loccN`/`on_hiccN` (and `start_` forms) for regions with no key, such as pedal noises.
  - `rt_decay`, in dB per second held.
- **Pitch:** `pitch_keycenter`, including `=sample`, which reads the WAV unity note. Also `pitch_keytrack`, `tune` (`pitch`) and `transpose`.
- **Level:**
  - `volume` (`gain`), `amplitude`, `pan`, `width`.
  - `amp_veltrack` and `amp_velcurve_N`. Without points the curve is velocity squared.
  - `amplitude_ccN`/`amplitude_onccN` multiply the gain by the CC's share of the amount.
  - `volume_onccN` and `pan_onccN` add linearly over the CC range.
- **Playback:**
  - `offset`, `offset_random`, `end` (`end=0` or `-1` silences a region), `delay`.
  - `loop_mode`, `loop_start`, `loop_end`. A loop stored in the WAV file is used when the SFZ names none.
  - `sample=*silence` regions play nothing but still trigger choke groups.
- **Envelope:** `ampeg_delay|start|attack|hold|decay|sustain|release`, plus their `_onccN`/`ccN` forms, which add linearly at note-on. Decay and release fall exponentially, reaching -96 dB at the release time.
- **Choke:** `group` (`polyphony_group`), `off_by`, `off_mode=fast|normal|time` and `off_time`.
- **Controls:** `set_ccN`, `set_hdccN` and `label_ccN`.
- **Ignored and reported:**
  - Filters, EQ, LFOs, and pitch and filter envelopes. Atlas's own filters and modulation apply after the oscillator.
  - `tune_ccN`. It is meaningless without its curves; Big Rusty Drums would be detuned by about 600 cents.
  - `*_curveccN` and `*_smoothccN`. CC modulation is linear.
  - `bend_up`/`bend_down` (Atlas's pitch-bend range applies), `polyphony`, and the generators `*sine`, `*noise` and so on.

## SF2 support

- **Structure:** `INFO`, `sdta` (`smpl` and `sm24`; 24-bit data is rounded to 16 bits) and `pdta`, with every index bounds-checked. ROM samples are skipped.
- **Zones:**
  - Instrument global and local zones merge.
  - Preset generators add to instrument ones; key and velocity ranges intersect.
  - Each zone becomes one mono region with its own pan, so stereo pairs need no linking.
- **Generators:**
  - Sample address offsets, including the coarse ones.
  - `sampleModes` 1 (loop) and 3 (loop until release).
  - `overridingRootKey`, `coarseTune`, `fineTune`, `scaleTuning` and the sample's pitch correction.
  - `pan`.
  - `initialAttenuation`, scaled by 0.4 as EMU hardware, FluidSynth and Polyphone do.
  - The volume envelope: `delayVolEnv`, `attackVolEnv`, `holdVolEnv`, `decayVolEnv`, `sustainVolEnv`, `releaseVolEnv`, `keynumToVolEnvHold` and `keynumToVolEnvDecay`.
  - `exclusiveClass` becomes a choke group that cuts itself.
- **Velocity:** follows the default velocity-to-attenuation modulator, so gain is velocity squared. Other modulators (`pmod`, `imod`) are ignored.
- **Presets:** every preset in the file is listed. Switching presets rebuilds only the regions; the sample pool is shared.

## Playback differences

- **Release and one-shot samples:** these sound past note-off. In the default Instrument amplitude-envelope mode, a slot that still sounds holds its voice open at full level, and Env 1 has no effect on that slot's volume. In Envelope 1 mode, Env 1 shapes and ends the voice as for any oscillator.
- **CC-triggered regions:** these have no voice. They play outside the voices, at full velocity from their key centre, to the slot's destination. Filter destinations go to the effects input unfiltered. Voice modulation doesn't reach them.
- **Choke latency:** a choke between voices can arrive one processing block late.
- **CC state:** all MIDI channels share it. All Controllers Off returns every CC to the instrument's `set_cc` default.

## Presets, host state and the cache

- **Preset files:** a preset whose Multi-sample oscillators have instruments is written as a zip:
  - `preset.json` comes first.
  - Then each instrument's files go under `multisample/<hash>/` in their original layout. Audio and SF2 files are stored uncompressed, exactly as they were; SFZ text is compressed.
  - SFZ text is never rewritten. Samples outside the library's folder go under `_external/` and are mapped back through `path_overrides`.
  - Other presets stay plain JSON.
- **The hash:** two FNV-1a hashes over each file's relative path, size and CRC32, so a library hashes the same wherever it lives.
- **Host (DAW) state:** stores the instrument's path and hash, not the audio. A restore tries, in order:
  1. The path. A changed file falls back to the saved copy and the user is told.
  2. The cache.
  3. Otherwise it reports the instrument missing and keeps the reference.
- **The cache:** `~/Library/Application Support/Atlas/Multi-sample Cache` on macOS and `%APPDATA%/Atlas/Multi-sample Cache` on Windows.
  - Container presets unpack into it, with each file's CRC checked.
  - Direct loads are hard-linked into it, or copied when under 2 GB.
  - It is trimmed, least recently used first, above 20 GB, never removing an instrument a slot uses.
  - Editing a cached SFZ works on a copy of its text in `Working/<hash>`.
- **Size limits:** JUCE's zip code has no ZIP64, so a container holds at most 4 GB and 65,000 files.

## Tests

`AtlasMultiSampleTests` (`tests/multisample_tests.cpp`) generates small WAV, FLAC, SFZ and SF2 fixtures. Each flag has an `atlas_multisample_*` ctest entry:

- Parsers: `--sfz-parser`, `--sf2-parser`, `--decode`.
- Playback through the plugin: `--pitch`, `--select`, `--envelope-mode`, `--release-trigger`, `--choke`, `--cc-trigger`.
- Saving and restoring: `--container`, `--host-state`, `--sf2-preset-switch`.
- Accessibility of the Instrument group: `--editor`.

Two flags are for checking real libraries by hand and are not in ctest:

- `--manual <file> [preset]` prints what the loader found.
- `--render <file> <out.wav> [preset]` plays a phrase and reports levels and speed.
