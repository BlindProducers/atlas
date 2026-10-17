# Speech oscillator

The Speech oscillator type sings the words typed into its text box, one
syllable per note, with the voice of eSpeak NG (`third_party/espeak-ng`, GPL v3
or later, used unchanged). Its controls are the text box, the Language and
Variant menus and the Amplitude Envelope mode, besides the Level, Pan and
Destination every slot has.

## How the words become notes

1. `speech::Manager` (message thread, `speech_manager.cpp`) notices a slot
   whose type is Speech and whose text, language or variant differs from what
   it plays, and renders it on its own thread. A timer watches the parameters,
   so host automation and preset loads re-render as editor changes do. Another
   type, or no text, frees the slot's phrase.
2. `espeak::speak` (`espeak_speech.cpp`) speaks the text with
   `espeak_SetVoiceByName("<language>+<variant>")` in a monotone
   (`espeakRANGE` 0) at 22,050 Hz, with phoneme events reporting IPA names
   (`espeakINITIALIZE_PHONEME_IPA`) and their sample positions. espeak-ng has
   one global state per process, which every plug-in instance shares, so every
   call holds one process-wide lock. It is never called on the audio thread.
   The Manager speaks each text five times, at espeak-ng pitch settings 6, 28,
   50 (the voice's own), 72 and 94 (`Manager::kLayerPitches`). espeak-ng's
   setting runs only from 0 to 99, about 57 to 158 Hz for the male voices and
   113 to 305 Hz for the female variants, roughly four semitones a step, and
   it raises the formants a little above 50 as a voice does.
3. `syllabify` (`speech_syllables.cpp`) splits each word's phonemes: every
   vowel, diphthong or syllabic consonant is a nucleus; a single consonant
   between two nuclei starts the next syllable, and of several the first ends
   the syllable before ("mon-ster"). Consonants before a word's first nucleus
   and after its last stay in that word's first and last syllables. A word
   without a nucleus is one syllable. espeak-ng's stress marks sit before the
   vowel, not at syllable boundaries, so they are not used to split.
4. `buildPhrase` cuts each recording at those boundaries (1.5 ms fade in,
   3 ms out) into a layer. `analyseLayer` (`src/synthesis/producers/
   speech_phrase.cpp`) measures the layer's monotone period over the vowels
   and places pitch marks: one per period on the larger peak of each voiced
   period, evenly spaced in unvoiced sounds (inside a vowel a weaker repeat
   counts as voiced). A layer is kept only when it splits into the same
   syllables as the voice's own pitch and its measured pitch rises with the
   setting. espeak-ng's language switches ("(en)") are not phonemes.
   Each syllable holds at the voiced mark 35% into its vowel (past its onset,
   on the first vowel of a diphthong). Its steady grain is two periods long:
   the average magnitude spectrum of the grains across 15% to 65% of the vowel
   with the phase of the grain at the hold point, windowed and as loud as that
   grain.
5. The Manager hands the phrase to the slot's `SpeechSlot` under the engine's
   lock. Notes already sounding finish with the previous phrase.

On the audio thread `SpeechSource` (TD-PSOLA) gives each note-on the slot's
next syllable; notes starting within 40 ms of each other (a chord) share one.
The note sings from the layer nearest its pitch, so inside espeak-ng's range
no grain moves more than about two semitones; beyond it the top or bottom
layer is shifted further. It lays Hann-windowed grains two source periods
long around the nearest mark, one note period apart in voiced sounds and at
the recording's spacing in unvoiced ones, so the formants stay where
espeak-ng put them. Grains are at most four note periods long, which bounds
the overlap far above the voice. The onset plays at speaking speed. At the
hold point, while the key is down, the grains crossfade over 80 ms into the
steady grain, repeated with 0.4% period jitter and 3% level shimmer per grain
so it does not sound frozen. On release they crossfade back over 40 ms and
the rest of the syllable plays from the hold point. After the last syllable
the words start again; host playback starting and All Sound Off rewind them.

The Amplitude Envelope mode, like Multi-sample's, OpenWurli's and OpenB3's:
Voice (the default) lets the syllable shape its note and holds the voice open
(`kHold`) until its last grain ends, so final consonants always sound;
Envelope 1 follows Env 1, whose release can cut them.

## The menus

`src/common/speech_voices.inc` lists the languages (`espeak-ng-data/lang`) and
variants (`espeak-ng-data/voices/!v`) by display name, written by
`generate_speech_voices.py`. Presets store positions in these lists, so when
espeak-ng is updated, run the script, keep the existing lines where they were
and move new entries to the end of their list. Variant 0 is "Default", the
language's own voice.

## Tests

`AtlasSpeechTests` (`tests/speech_tests.cpp`): `--syllables` (the splitter on
made-up events and on espeak-ng's own in English, German and Spanish),
`--phrase` (the layers' pitches, marks, hold points and steady grains for
three voices), `--pitch` (the sung pitch at four notes, and a steady held
vowel), `--envelope` (Voice sings the final consonant after release,
Envelope 1 with no release cuts it), `--sequence` (which
syllable each note takes, chords, rewinding, phrase changes), `--parameters`
(host state) and `--editor` (the text box and the two menus). The test binary
reads the data from `build/espeak-ng`. `--render <file.wav>` writes the start
of "Happy Birthday" to listen to; it is not a ctest.
