# Multi-sample oscillator (SFZ, DecentSampler and SF2)

The Multi-sample oscillator type plays SFZ, DecentSampler (`.dspreset`) and
SoundFont 2 instruments. The parsers, loader and player are written for Atlas
from the public format descriptions: the SoundFont 2.04 specification, the
opcode reference at sfzformat.com and the DecentSampler Developer Guide
(decentsampler-developers-guide.readthedocs.io, version 1.37). No code from
another SFZ, DecentSampler or SF2 player is used, so `NOTICE` is unchanged.
The DecentSampler converter in schwung-sfz (github.com/charlesvestal/schwung-sfz)
was read as a second description of the format, but none of its code is used.
Behaviour matches sfizz and FluidSynth where noted below.

## Files

- `multisample_instrument.*`: the immutable instrument model (regions, lookups, CC defaults).
- `sfz_parser.*`, `decentsampler_parser.*`, `sf2_parser.*`: SFZ text, DecentSampler XML and RIFF parsing. `resolveInstrumentPath` in `sfz_parser.*` finds sample files for both text formats.
- `multisample_loader.*`: decoding (WAV, AIFF, FLAC, Ogg) to 16-bit, CRC32 and the content hash.
- `multisample_container.*`: self-contained presets and the instrument cache.
- `multisample_manager.*`: background loading, per-slot state, presets and host state.
- Playback is in `src/synthesis/producers/multisample_{player,slot,source}.*` and `src/synthesis/modules/multisample_module.*`.

## SFZ support

- **Headers:** `<control>`, `<global>`, `<master>`, `<group>` and `<region>`, inherited in that order. A new header resets the levels below it. `<effect>`, `<midi>` and `<sample>` blocks are skipped and reported. Spaces may stand around an opcode's `=` (`volume = -3`, as The Experience NY S&S Model B writes it).
- **Curves:** `<curve>` blocks with `curve_index` and `vN` points, interpolated linearly, with 0 at the start and 1 at the end unless given. SFZ's built-in curves 0 to 6 (linear, bipolar, inverted, inverted bipolar, squared, square root, inverted square root) need no block. `amplitude_`, `volume_`/`gain_`, `pan_` and every `ampeg_` stage take a `_curveccN`, before or after the modulation it shapes. An undefined curve reads as linear and is reported.
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
- **Envelope:** `ampeg_delay|start|attack|hold|decay|sustain|release`, plus their `_onccN`/`ccN` forms, which add at note-on through their curve (linearly without one). With `ampeg_dynamic=1` the release and sustain follow their CCs while the note plays, and a release under way goes on at the new rate. The Experience NY S&S Model B lengthens the release to 15 s with the pedal down this way, and lifting the pedal damps the strings. Decay and release fall exponentially, reaching -96 dB at the release time.
- **Choke:** `group` (`polyphony_group`), `off_by`, `off_mode=fast|normal|time` and `off_time`.
- **Controls:** `set_ccN`, `set_hdccN` and `label_ccN`.
- **Ignored and reported:**
  - Filters, EQ, LFOs, and pitch and filter envelopes. Atlas's own filters and modulation apply after the oscillator.
  - `tune_ccN`. It is meaningless without its curves; Big Rusty Drums would be detuned by about 600 cents.
  - `*_smoothccN`, and `ampeg_release_shape` (releases fall exponentially).
  - `sustain_cc`. Atlas's sustain pedal (CC64) holds the whole voice, so a library that moves its sustain elsewhere and lets CC64 shape the release (The Experience NY S&S Model B) gets its note-offs when the pedal lifts. With `ampeg_dynamic` the release then takes the pedal-up length and damps, as the library intends; its release-triggered damper and key noises play at that moment rather than at key-up.
  - `bend_up`/`bend_down` (Atlas's pitch-bend range applies), `polyphony`, and the generators `*sine`, `*noise` and so on.

## DecentSampler support

A `.dspreset` becomes the same regions an SFZ file would. Samples are found the
same way, from the preset's folder, ignoring case when the exact path is missing.

- **Structure:** `<groups>`, `<group>` and `<sample>`. Most attributes inherit from `<groups>` to `<group>` to `<sample>`. A group with `enabled="false"` plays nothing, but its samples are still part of the instrument: the parser reports them (`SampleReference::played` false), and the loader stores and hashes them without decoding them, so a preset carries the whole library and a control that enables the group later finds its files. The same holds for samples a disabled tag silences.
- **Mapping:** `rootNote`, `loNote`, `hiNote`, `loVel`, `hiVel`, `loCCN`/`hiCCN` (a value of -1 is off), and `trigger=attack|release|first|legato`. `continuous` plays like `attack` and is reported. `onLoCCN`/`onHiCCN` start CC-triggered regions.
- **Round robins:** `seqMode=round_robin` uses `seqPosition` and `seqLength`. `random` and `true_random` split the random range by position. Without `seqLength`, the length is the highest `seqPosition` among the group's samples that share keys and velocities with the sample.
  - Without any `seqMode`, DecentSampler's default is `always`, which plays every matching zone. Samples that share keys and velocities but have different `seqPosition` values play as `round_robin` instead, and this is reported. Numbering takes without a mode is a mistake in the preset (DecentSampler's developer calls it a bug in its converters), and stacking the takes is louder and smears the attack. Claustrophobic Piano is one library written this way. An explicit `seqMode="always"` still stacks.
- **Pitch:** `globalTuning`, `groupTuning` and `tuning` add up in semitones. `pitchKeyTrack` scales key tracking and, as in DecentSampler, also scales the tuning unless `tuningIgnoresKeyTrack="true"`.
- **Level:**
  - The `<groups>` volume (`globalVolume`), the `<group>` volume (`groupVolume`) and the `<sample>` volume multiply. Each is linear or ends in `dB`.
  - `pan` inherits; `globalPan` and `groupPan` add to it, clamped to ±100.
  - `ampVelTrack` (default 1) with gain linear in velocity, so velocity 64 plays at about half.
- **Playback:** `start`, `end` (inclusive), `delay` in seconds (`delayUnit=beats|samples` is reported), `loopEnabled`, `loopStart`, `loopEnd` (inclusive), `loopMode="pingpong"` (the loop plays forward to its last frame and back to its first; other modes play forward and are reported) and `loopDuringRelease="false"` (loop until release). Without `loopEnabled`, a loop stored in the WAV file is used, as for SFZ. `ampEnvEnabled="false"` plays to the end of the sample.
- **Envelope:** `attack`, `decay`, `sustain` and `release`. DecentSampler documents no defaults; Atlas uses no attack or decay, full sustain and a 0.1 s release.
- **Release samples:** `releaseTriggerDecay` in dB per second (`"3dB"`), or a linear share lost per second.
- **Tags:**
  - `tags` on groups and samples. `<tags>` sets each tag's `volume` and `enabled`.
  - `silencedByTags` becomes a choke group, with `silencingMode=fast|normal` and `silencingDecay`. Atlas regions have one choke group each, so a sample with several silencing tags chokes through the first one, and a `silencedByTags` list uses its first tag. Both cases are reported.
- **Controls:** DecentSampler fires each control's bindings at load and whenever the control moves. Atlas does the same. A value replaces the attribute at the binding's level, so a sample's own `release` still wins over an instrument `ENV_RELEASE` knob. `AMP_VOLUME` is the exception: as in DecentSampler, the knob's level multiplies the `volume` the file sets. A release group at `volume="20dB"` under a knob at 0.6 plays at 6 times, which is how Claustrophobic Piano's quiet key-release samples are heard. schwung-sfz's converter, checked against DecentSampler renders, does the same. Mapping parameters (notes, velocities, sample and loop points) set every sample below the binding.
  - `<labeled-knob>`, `<control>` and `<slider>` use their `value`, `<menu>` its selected `<option>` (counting from 1), and `<button>` its selected `<state>` (counting from 0).
  - Translations `linear` (with `translationOutputMin`/`Max` and `translationReversed`), `table` and `fixed_value`, after `factor`. Bindings with `enabled="false"` are skipped, and so are bindings with `triggerOnLoad="false"` until the user moves their control.
  - **In the editor:** each visible control with at least one binding Atlas applies appears in the Instrument group. A knob becomes a slider and a menu or button becomes a menu, titled by the control's `label`, then `parameterName` or `text`, then the `<label>` the panel shows on it: the one whose text (centred in its box) lies within the control's width, at its top edge or up to its height above it. Files list labels before or after their controls (Emerald Resonances puts them after), so position decides, and only a panel without coordinates falls back to a label just before the control. Controls with no name are numbered by kind among those listed ("Menu 1"). A 0 to 1 knob reads as a percentage, and `type="integer"` knobs step by whole numbers. Backspace returns a control to the file's value.
  - **Changing a control** parses the preset again with the new value (`SfzParser::Options::control_values`) on the loading thread. The decoded audio is reused and the slot's status does not change. Samples of a group the change enables are decoded then, and the audio of a group it disables is kept for the next time. The hash and the files a preset stores are the same whatever the controls are set to. The engine swaps instruments softly: notes already sounding play on with the previous instrument, which the slot keeps until the next change, and new notes use the new one.
  - **Saving:** the values the user chose are stored by the control's position in the file (`controls` in the slot's instrument spec), in presets and host state. Loading the file again from disk starts from its own values.
  - Instrument level: `AMP_VOLUME`, `GLOBAL_TUNING`, `PAN`, the `ENV_*` stages, `AMP_VEL_TRACK`, `PITCH_KEY_TRACK`, `SAMPLE_START`/`END`, `LOOP_START`/`END`, `LO_NOTE`/`HI_NOTE`, `ROOT_NOTE` and `SILENCING_MODE`/`DECAY`.
  - Group level, by `position`/`groupIndex` or `groupTags`/`tags`: the same, plus `AMP_VOLUME`, `GROUP_TUNING`, `PAN` (the group pan offset), `ENABLED`, `AMP_ENV_ENABLED`, `LO_VEL` and `HI_VEL`.
  - Tag level: `TAG_VOLUME` and `TAG_ENABLED`, also written `AMP_VOLUME` and `ENABLED`.
- **Ignored and reported:**
  - `<effects>`, `<modulators>`, `<midi>` (so CCs don't move controls, and controls are not host parameters), `<buses>`, `<noteSequences>`, `<arpeggiator>` and `<oscillator>`.
  - Effect, modulator and other bindings.
  - Envelope curves, `loopCrossfade` (in samples: values under one, as Emerald Resonances writes, are no crossfade and are not reported), glide, slice stretch, `previousNotes`, `legatoInterval`, output routing and tag polyphony.
  - `length` and `playbackMode` are skipped silently: they describe the file and how DecentSampler streams it, not the sound.

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
  - Then each instrument's files go under `multisample/<hash>/` in their original layout. Audio and SF2 files are stored uncompressed, exactly as they were; SFZ and DecentSampler text is compressed. Every sample file the instrument names and that exists is stored, including DecentSampler samples in groups and tags that are disabled at the time; only missing files are left out.
  - SFZ and DecentSampler text is never rewritten. Samples outside the library's folder go under `_external/` and are mapped back through `path_overrides`.
  - Other presets stay plain JSON.
- **The hash:** two FNV-1a hashes over each file's relative path, size and CRC32, so a library hashes the same wherever it lives, and the same whatever its controls are set to. Presets and host state saved before Atlas stored the samples of disabled groups hash without those files. Such a preset still plays from its saved copy, which lacks them, so enabling one of those groups finds nothing until the instrument is loaded again from its library; host state restored from the original file reports the instrument as changed and plays the saved copy while the cache has it.
- **Host (DAW) state:** stores the instrument's path and hash, not the audio. A restore tries, in order:
  1. The path. A changed file falls back to the saved copy and the user is told.
  2. The cache.
  3. Otherwise it reports the instrument missing and keeps the reference.
- **The cache:** `~/Library/Application Support/Atlas/Multi-sample Cache` on macOS and `%APPDATA%/Atlas/Multi-sample Cache` on Windows.
  - Container presets unpack into it, with each file's CRC checked.
  - Direct loads are hard-linked into it, or copied when under 2 GB.
  - It is trimmed, least recently used first, above 20 GB, never removing an instrument a slot uses.
  - Editing a cached SFZ or DecentSampler preset works on a copy of its text in `Working/<hash>`.
- **Size limits:** JUCE's zip code has no ZIP64, so a container holds at most 4 GB and 65,000 files.
  - A sample file has no length cap: DecentSampler instruments often slice one recording of most of an hour. A WAV or AIFF must hold every frame its header claims, and no file may decode to more than the instrument's memory budget (70% of RAM).

## Tests

`AtlasMultiSampleTests` (`tests/multisample_tests.cpp`) generates small WAV, FLAC, SFZ, DecentSampler and SF2 fixtures. Each flag has an `atlas_multisample_*` ctest entry:

- Parsers: `--sfz-parser`, `--decentsampler-parser`, `--sf2-parser`, `--decode`.
- DecentSampler end to end (load, pitch and level, container, restore, editing a cached copy): `--decentsampler-load`.
- DecentSampler controls (rebuilds, held notes across a change, saving the values, and a group a button disables whose samples are stored, hashed and found again after a restore): `--decentsampler-controls`.
- Playback through the plugin: `--pitch`, `--select`, `--envelope-mode`, `--release-trigger`, `--choke`, `--cc-trigger`.
- Ping-pong loops in the player: `--pingpong-loop`.
- SFZ curves, spaces around `=` and `ampeg_dynamic` with the sustain pedal: `--dynamic-envelope`.
- Saving and restoring: `--container`, `--host-state`, `--sf2-preset-switch`.
- Accessibility of the Instrument group: `--editor`.

Five flags are for checking real libraries by hand and are not in ctest:

- `--manual <file> [preset]` prints what the loader found, including the instrument's controls.
- `--controls <file>` plays a held C4 with each control at its lowest, file and highest setting (every option of a menu) and prints the levels.
- `--compare <a> <b> [mapping]` lists, for every key and several velocities, the samples two files of one library play (an SFZ and its DecentSampler version) and how, and prints where they differ. `mapping` leaves out gain and envelope, which the two formats set differently.
- `--hold <file> <note> <seconds>` holds a note and prints the level and largest sample step per 5 s window, to hear loops turn.
- `--render <file> <out.wav> [preset]` plays a phrase and reports levels and speed.
