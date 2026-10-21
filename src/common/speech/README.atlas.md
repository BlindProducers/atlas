# Speech oscillator

The Speech oscillator type sings the words typed into its text box, one
syllable per note, with the voice of eSpeak NG (`third_party/espeak-ng`, GPL v3
or later, used unchanged). Its controls are the text box, Mode, Split, the
Language and Variant menus, Formant, Breathiness, Speed, Vibrato Depth and
Rate, and the Amplitude Envelope mode, besides the Level, Pan and Destination
every slot has. Pitch and unison holds Transpose (semitones) and
Tune (cents), added to the note before a layer is chosen. Warp holds the
slot's FM and ring modulation from other oscillators (`SlotModulation`): FM
scales how fast grains are laid, not how fast the syllable is read, so it
bends the pitch at audio rate while the formants and the syllable's timing
stay put, and never runs backwards; ring modulation multiplies the output.
Speech has no unison or scale lock.

- **Mode.** Sung renders a monotone and sings each note's pitch, holding the
  vowel while the key is down. Spoken renders with espeak-ng's own intonation
  (`espeakRANGE` 50) and plays straight through; middle C
  (`SpeechSource::kSpokenRootNote`) speaks at the reference layer's pitch,
  and other notes move the whole contour by their interval. Its marks follow
  the moving pitch: frames search 0.55 to 1.8 times the layer's period and
  take the shortest lag that repeats within 85% of the best. A note sings
  from the layer nearest its target, and each voiced grain follows the next
  at the recording's local period divided by the note's ratio to that layer.
  Changing the mode renders the text again.
- **Split.** Each note takes the next syllable, word (`Phrase::word_starts`,
  from espeak-ng's word events) or the whole text. A note plays its
  syllables in turn at speaking speed; only the last one holds its vowel.
  The song-position counting of the next section counts these units.
- **Formant** (semitones) reads each grain faster or slower while the grains
  stay a note period apart, which moves the formants and keeps the pitch.
  `SynthBase::valueChanged` sets it and Breathiness back to 0 when the
  slot's variant changes (in the editor or from the host), not its
  language; preset and project loads set controls directly, so saved values
  survive them.
- **Breathiness** crossfades (at equal power) every grain into the same span
  of the syllable's whispered twin, `Syllable::breath`: built off the audio
  thread frame by frame (512-point frames a quarter apart) as noise with the
  frame's level and its cepstral envelope below 0.4 of the voice's period
  (12 to 40 quefrency bins), so it keeps the formants without the harmonics.
  Voiced grains read their whisper near their mark, held ones anywhere in
  the vowel's middle, so the noise never repeats at the note's period.
- **Speed** (0% to 400%) scales how fast the recording is read between
  holds; the pitch stays. 100% is 65% of espeak-ng's speaking speed, which
  sings too fast, and 0% still reads at 2% so a released note finishes.
  Reading slowly spreads unvoiced grains around the read position so
  consonants do not buzz.
- **Vibrato** fades in from 150 ms after the note over 350 ms, depth in
  cents either side, rate 1 to 12 Hz.

## How the words become notes

1. `speech::Manager` (message thread, `speech_manager.cpp`) notices a slot
   whose type is Speech and whose text, language, variant or mode differs from what
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
next syllable; notes starting within 40 ms of each other (a chord) share one,
and so does a note pressed while another key is down: the text moves on only
once every key is up. Before the voices render each block, the voice handler
tells each slot how many keyboard notes are down and where in the block any
are released (`SpeechSlot::setHeldNotes`), so a key released on the sample
where the next note starts doesn't hold its syllable. Notes held only by the
sustain pedal, and notes an arpeggiator plays, don't hold the syllable.
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
the words start again; All Sound Off rewinds them.

## Following the song

While the host plays, the syllables follow the song position, so starting
playback or moving the playhead anywhere sings the syllable that a play from
the beginning would. Each `SpeechSlot` remembers where in the song, in
quarter notes, its notes started (the first note of each chord, up to
`kMaxSongNotes`) and which stretches of the song have played (up to
`kMaxPlayedRanges`), both reserved off the audio thread. Before every block
the host plays, `SynthBase::processAudioAndMidi` gives the slots the block's
song position and the host's loop; the slot forgets the notes it remembered
in that stretch and marks it played, and each note that plays there is
remembered again and sings the syllable counted by the notes before it
(modulo the number of syllables). So the slot learns the song as it plays:
until a stretch has played once, notes in it are not counted, and a note
moved or deleted in the host is relearned the next time playback passes it.
Positions are in quarter notes, so tempo changes do not move them, and
changing the text keeps the song's place.

Atlas works out positions inside a host block by counting forward from the
block's start, so when the host's loop returns partway through a block, the
positions after it wrap to the loop start rather than running past the loop
end.

Hosts that chase notes (Reaper, Cubase and others) send a note held across
the point where playback starts, or where a loop returns, as a new note-on
there. When the host starts or jumps (a block that does not begin where the
last one ended, or the loop returning inside a block), a note-on exactly at
that point (within `kSamePlace`) in a stretch already played, where no note
was remembered starting, is taken as chased: it sings the syllable of the
last note before it and is not remembered. In a stretch never played it
counts as a note. A note newly added exactly at such a point is taken as
chased until playback passes over it from before.

With the transport stopped (or a host without a song position), each note
takes the next syllable after the last one sung, and playback without a song
position starts from the first. The notes and played stretches are saved
with the host's project (`speech_song` in `SynthPlugin::getStateInformation`),
not in presets.

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
vowel), `--tuning` (Transpose and Tune), `--slot-modulation` (FM and RM),
`--split` (word and phrase units and their held last vowel), `--spoken`
(intonation, middle C at the voice's pitch, an octave up, playing through),
`--voice-controls` (formant, breath, speed and vibrato, and their reset on
a new variant), `--envelope` (Voice sings the final consonant after release,
Envelope 1 with no release cuts it), `--voice-kill` (a voice stolen for a
new note fades out instead of singing the rest of its phrase), `--legato` (a mono legato note keeps
its syllable, without Legato each note takes the next), `--sequence` (which
syllable each note takes, chords, rewinding, phrase changes), `--transport`
(syllables at song positions after moving the playhead, chased notes, loops
returning inside a block, chords, edited notes, the host's saved state), `--parameters`
(host state) and `--editor` (the text box, the menus and every row, in
order). The test binary
reads the data from `build/espeak-ng`. `--render <file.wav>` writes the start
of "Happy Birthday" to listen to; it is not a ctest.
