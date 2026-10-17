/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ATLAS_OPENEVV_H
#define ATLAS_OPENEVV_H

/* What Atlas needs from OpenEVV beyond its published ECI interface: the
   parameter frames a render hands the Klatt synthesiser, and that
   synthesiser on its own, for the Speech oscillator's voices to play those
   frames again at any pitch. Built into the atlas_openevv library
   (cmake/openevv.cmake), where OpenEVV's own headers are visible. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One frame is the synthesiser's 62 words and the duration slot after them,
   in the order of src/klatt/klatt_synth.c (step in milliseconds, f0 in
   tenths of a hertz, the amplitudes in decibels, ...). */
#define ATLAS_KLATT_FRAME_WORDS 63

/* Bytes a voice keeps for one synthesiser. The state has no pointer into
   anything but itself and static tables, and atlas_klatt_open sets every one
   of them, so it may be copied (a cloned voice) and opened again. */
#define ATLAS_KLATT_STATE_BYTES 8192

/* The four voice offsets in decibels (voicing, aspiration, frication and the
   bypass, KlattConstParms' unknown_20, _24, _28 and _2c) and the formant
   count a render's rules gave the synthesiser. */
typedef struct {
    int32_t add_20;
    int32_t add_24;
    int32_t add_28;
    int32_t add_2c;
    int32_t n_formants;
} AtlasKlattVoice;

/* A run of samples, at most a frame's (220 at 44,100 hertz). */
typedef void (*AtlasKlattSink)(void *user, const int32_t *samples, int32_t count);

/* Each frame the engine synthesises during a render, with the voice and the
   volume (per cent) it was synthesised with. */
typedef void (*AtlasFrameTap)(void *user, const int32_t *parms, const AtlasKlattVoice *voice, int32_t volume);

/* Starts the engine's platform layer and static initialisers once, and builds
   the 22,050 hertz resonator tables. Any thread; later calls return at once. */
void atlas_openevv_start(void);

/* Sets or clears (null) where the frames of a render go. Only one render runs
   at a time; the voices' own synthesisers are never reported. */
void atlas_openevv_set_tap(AtlasFrameTap tap, void *user);

/* Opens a synthesiser in `state` (ATLAS_KLATT_STATE_BYTES) at 11,025 or 22,050
   hertz, ready for frames. No allocation: the audio thread may call it.
   Answers 0 for another rate. */
int atlas_klatt_open(void *state, int32_t rate, const AtlasKlattVoice *voice, int32_t volume,
                     AtlasKlattSink sink, void *user);
/* Points the synthesiser's samples somewhere else: a voice whose memory was
   copied names itself again before each frame. */
void atlas_klatt_set_sink(void *state, AtlasKlattSink sink, void *user);
/* Changes the volume (per cent) between frames, as the rules do. */
void atlas_klatt_set_volume(void *state, int32_t volume);
/* Synthesises one frame into the sink. Answers 0 if it was refused. */
int atlas_klatt_synth(void *state, const int32_t *parms);
/* Samples one frame lasts at the open rate. */
int32_t atlas_klatt_frame_samples(const void *state, const int32_t *parms);

#ifdef __cplusplus
}
#endif

#endif
