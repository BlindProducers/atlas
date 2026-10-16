# Tape noise recordings

The Tape effect's Noise section loops one of these recordings, chosen with
Tape Noise Sound (`Vinyl 1` to `Vinyl 8`, `strings::kTapeNoiseNames`). They
are plain 24-bit stereo WAV files at their source's sample rate.
`TapeNoiseLibrary` (`src/common/tape_noise_library.cpp`) reads them from the
copy of this folder in the user's data directory, or straight from the
plug-in bundle's `Resources/Tape`, where the build copies them. Atlas 2.0.0
beta 1 shipped 27 encrypted clips of unrecorded origin; these replace them.

| File | Source | Author | Licence | Original | Changes |
| --- | --- | --- | --- | --- | --- |
| Vinyl 1 | [Vinyl noise](https://freesound.org/s/837986/) | Artem_Zaitsev | CC0 1.0 | 24-bit 48 kHz WAV, 60 s | gain -3.7 dB |
| Vinyl 2 | [Analog Noise](https://freesound.org/s/701692/) | Ihaksi | CC0 1.0 | 24-bit 44.1 kHz WAV, 20 s | gain -12.2 dB |
| Vinyl 3 | [Vinyl_Dust-Electrical-Mechanical-Noise_1980_IzotopeVinyl](https://freesound.org/s/678794/) | FkaLu | CC0 1.0 | 32-bit float 44.1 kHz WAV, 30 s | gain -4.0 dB, 24-bit |
| Vinyl 4 | [Vinyl Crackle 33RPM](https://freesound.org/s/471007/) | yfjesse | CC0 1.0 | 16-bit 44.1 kHz WAV, 5 min 4 s | first 60 s, gain +4.8 dB, 24-bit |
| Vinyl 5 | [Dirty Record](https://freesound.org/s/164011/) | zeffon | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | 16-bit 44.1 kHz WAV, 74 s | gain -5.5 dB, 24-bit |
| Vinyl 6 | [Vinyl Hiss](https://freesound.org/s/68400/) | DanielsonIII | CC0 1.0 | 32-bit float 44.1 kHz WAV, 30 s | gain +20.6 dB, 24-bit |
| Vinyl 7 | [Vinyl - 45RPM - end 2](https://freesound.org/s/619278/) | day-garwood | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | 16-bit 44.1 kHz FLAC, 55 s | WAV, gain -12.7 dB, 24-bit |
| Vinyl 8 | FX 098, QuoraX x Hamo 2026 Stash sample pack | @quoraxmusic, @hamomadeit | the pack's own terms | 32-bit float 44.1 kHz WAV, 5.4 s | gain +0.6 dB, 24-bit |

CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) places no
conditions on use. CC BY 3.0 asks for the credit above, a link to the licence
and a note of the changes made; `NOTICE` repeats the credits. Two files
deserve a note before a release ships them: Vinyl 3 was recorded from a
commercial plug-in's noise generator (its uploader names Arturia's Tape
Mello-Fi, and its file name lists iZotope Vinyl settings), so the CC0 grant
covers only what the uploader could license; and Vinyl 8 comes from a
commercial sample pack whose redistribution terms were not available when it
was added, so they must be confirmed.

## Loudness matching

Every file is matched to -36 LUFS integrated loudness (EBU R128, measured
with ffmpeg's `ebur128` filter) with a plain gain, so that switching sounds
keeps the level. That is the highest common level at which no recording's
true peak exceeds -1 dBFS; Vinyl 4's crackle, 35 dB above its loudness, sets
it. True peaks after matching range from -1.2 dBFS (Vinyl 4) to -26.9 dBFS
(Vinyl 2). Nothing else was processed: no limiting, filtering, fades or
resampling. The module loops each file with its own 8 ms crossfade.

To regenerate a file from its original, with `<gain>` the difference between
-36 LUFS and the original's integrated loudness (`ffmpeg -i original -af
ebur128=peak=true -f null -` prints it):

    ffmpeg -i original [-t 60] -af volume=<gain>dB -map_metadata -1 -fflags +bitexact -c:a pcm_s24le "Vinyl <n>.wav"
