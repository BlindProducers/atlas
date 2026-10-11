# Spectral Lock source

Spectral Lock is by aquanodemusic and is published under the AGPL-3.0 at
https://github.com/aquanodemusic/aquanode-plugins (`spectral/spectrallock/src`).
`LICENSE` is that repository's licence. The four source files here are the
SpectralLock 1.0.0 processor and editor as supplied for this integration;
`source_sha256.json` records their hashes. `PluginProcessor.cpp`,
`PluginEditor.cpp` and `PluginEditor.h` are byte-identical to the upstream
`src` folder at the time of checking (October 2026); `PluginProcessor.h`
differs only in the program name string returned by `getProgramName`.

Atlas extracts the filterbank, oscillators, envelopes, gates, crossover filters,
scale tables, mix and smoothing into `src/synthesis/effects/spectral_lock`
(also AGPL-3.0). The sample loop and its arithmetic retain the source behavior.
Unexposed controls stay at source defaults: Glide 0, Level 1, MIDI off, Freeze
off. Width (held at 1 in the source) is an Atlas control, and Atlas adds
Follow, which holds the tonal output at or below the dry signal's envelope.
Width glides per block (20 ms) instead of stepping, and a non-allocating
reset clears the engine when Atlas switches Spectral Lock's Mode.
Scale selects the original matrix preset; Root keeps its original transposition.
Display/meter-only calculations and APVTS/editor overhead are omitted. Invariant
band envelopes and pan coefficients are prepared once; mappings are recomputed
only when their inputs change. Each SIMD stereo pair and FX slot owns its state.
The engine is compiled separately from Atlas's fast-math synthesis unity build.
The original processor is compiled independently as the regression oracle.

Latency remains zero, as in the supplied processor: envelope response and filter
phase are part of this effect's sound, not a fixed buffering delay to compensate.
