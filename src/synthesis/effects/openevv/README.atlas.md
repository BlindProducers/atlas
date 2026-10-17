# OpenEVV and the Speech oscillator

The Speech oscillator type speaks text with OpenEVV
(https://github.com/Mudb0y/openevv, commit d2bc0c6): IBM's Embedded ViaVoice,
the text-to-speech engine screen reader users know as Eloquence, rebuilt as
portable C. Each key plays the next syllable, word or the whole text at the
key's pitch.

## Licence: read this before shipping a build

OpenEVV's engine (`src`, `lib`, `tools`) is MIT licensed (Copyright (c) 2026
Stanislaw Przedzinkowski, the OpenEVV SAPI5 contributors and Felix
Steindorff), which the GPL v3 can carry. **Its language data is not.** Every
module under `lang`, and the two lifted tables in `src`
(`klatt/klatt_tables.c`, `eci/ssml/eci_xmltok_tables.c`), is IBM's or ETI's,
transcribed from their objects, and OpenEVV's NOTICE says plainly that nobody
there can license it. A build of Atlas with the engine in it therefore
contains material that neither Atlas's GPL nor anyone else grants a right to
redistribute.

So the engine is never in Atlas's own tree: `third_party/openevv` is an opt-in
submodule (`update = none`), which the release workflow does not fetch, and a
build without it has the Speech type with no engine (it stays silent and says
why). Fetch it only for builds you keep to yourself, unless you have sorted out
the rights in that data.

## What Atlas builds

`cmake/openevv.cmake` builds what OpenEVV's Makefile builds with
`RULES=bytecode` (rules interpreted, a 16-second build of all ten languages
rather than a quarter of an hour): the engine, the language modules listed in
`ATLAS_OPENEVV_LANGUAGES`, and the rule code OpenEVV's Python tools write out
of `lang/<tag>/rules` (`cmake/openevv_rules.cmake`; the files land where
OpenEVV's `.gitignore` covers them). MSVC cannot compile OpenEVV's GNU C, so
Windows builds have no engine for now.

Atlas changes three sources and the rule shims on their way into the build
tree, leaving the submodule untouched:

- **Apple arm64.** OpenEVV runs on Linux aarch64 but faulted on its first
  instance on Apple Silicon. Its interpreter calls every rule and primitive
  through a pointer whose arguments are whole machine words. Apple's arm64
  passes a variadic function's arguments on the stack, where that call leaves
  them in registers, so `setNonSequential` (variadic) read garbage; Atlas gives
  it fixed arguments. And Apple packs stack arguments to their own size rather
  than to eight bytes, so a rule shim declared with `int32_t` arguments read
  its ninth and tenth from the wrong half of a slot; Atlas declares them
  `uintptr_t` and narrows. With both, the engine's output is byte-identical to
  OpenEVV's recorded hash (`test/hash.sh`) on an M-series Mac. Both are worth
  sending upstream.
- `src/klatt/klatt_synth.c` also hands every parameter frame to
  `atlas_openevv_klatt_frame` (`atlas_openevv.c`), which records the frames of
  a render.
- `lib/eci_api.c` loses its constructor; Atlas starts the engine the first time
  it renders.

## How the oscillator works

1. **Render** (`openevv_engine.cpp`, on SynthBase's `SpeechManager` thread).
   The text is spoken through the published ECI interface with the slot's
   eight voice controls, word by word with an index mark after each. Every
   Klatt parameter frame the engine synthesises (5 ms, 62 parameters) is
   recorded with the volume and voice offsets the rules gave it. One instance
   per render, so a render is deterministic; renders from all plugin
   instances take turns. A change to the text or a voice control renders
   again; a preset renders before it plays.
2. **Split** (`segment`). Each word starts at its index mark, which the
   engine reports within a frame or two of the word (or as the word before is
   still running into its consonant; up to a short vowel early after a word
   like "a"): the cut moves to the deepest dip just after the mark, with every
   frame away from it counting against a dip, so a word is never lost or run
   into the next ("out of" is two keys). Inside a word, syllable nuclei are
   sonority peaks (voicing plus a little for an open first formant, less for
   frication) with a 4 dB dip between them and at least 60 ms apart at the
   usual speed. Frames without the aspiration the rules mix into this voice's
   vowels (nasals, voiced fricatives), with more of it (the breathy end of a
   phrase) or with tilted voicing (voiced closures) count as consonants. The
   speed setting changes how many frames a syllable takes (measured: 18% fewer
   for every 10 steps above 50), and the spacing, the search after each mark
   and the smoothing follow it. A syllable boundary is in the dip: at the
   first fricative or burst there (it begins the next syllable), else halfway.
   Vowels meeting with no consonant between ("video") stay one syllable. The
   frame a held key sustains is the middle of the vowel's steady part, clear
   of the creaky onset (diplophonia) Eloquence gives a vowel.
3. **Play** (`speech_source.cpp`). Each voice owns a Klatt synthesiser
   (OpenEVV's own, `atlas_klatt_*`, in 8 KB of the voice; no allocation on the
   audio thread) and plays the piece's frames with the key's pitch in place
   of the spoken one (plus Intonation's share of the spoken melody) and the
   rate. A legato key only moves the pitch: the voice stays on its piece. It runs at 11,025 hertz (Eloquence's own sound)
   or 22,050 (OpenEVV's tables built for that rate; steadier high notes), and
   a windowed sinc resamples to the voice's rate. Played through unchanged,
   the frames reproduce the engine's own samples exactly.

## Tests

`AtlasSpeechTests` (`tests/speech_tests.cpp`), built when the engine is:
`--engine` (frame replay against the engine's samples, determinism),
`--split` (syllable and word counts of known phrases), `--languages`,
`--voice`, `--oscillator` (pitch at both rates, release, Spoken mode),
`--stepping` (a piece per key, chords, Restart, Loop, Restart on release,
re-renders), `--state` and `--editor`. `--dump "<text>"` prints each frame
with the pieces, for tuning the splitter.
